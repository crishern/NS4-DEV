/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
* Copyright (c) YEAR COPYRIGHTHOLDER
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation;
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*
* Author:
*/

#include "ns3/p4-model.h"
#include "ns3/helper.h"
#include "ns3/global.h"
#include "ns3/log.h"
#include "ns3/node.h"
#include "ns3/ethernet-header.h"
#include "ns3/arp-l3-protocol.h"
#include <bm/bm_sim/switch.h>
#include <bm/bm_sim/core/primitives.h>
#include <bm/bm_runtime/bm_runtime.h>
#include <bm/bm_sim/simple_pre.h>
#include <bm/SimpleSwitch.h>
#include <bm/bm_sim/options_parse.h>

using namespace ns3;

NS_OBJECT_ENSURE_REGISTERED(P4Model);
//NS_LOG_COMPONENT_DEFINE(P4Model);

TypeId P4Model::GetTypeId(void)
{
	static TypeId tid = TypeId("ns3::P4Model").SetParent<Object>().SetGroupName(
		"Network")
		// .AddConstructor<P4Model> ()
		;
	return tid;
}

using bm::McSimplePre;
extern int import_primitives();

P4Model::P4Model(P4NetDevice* netDevice) :
	m_pre(new bm::McSimplePre()) {

	m_pNetDevice = netDevice;

	add_component<bm::McSimplePre>(m_pre);

	m_argParser = new bm::TargetParserBasic();
	add_required_field("standard_metadata", "ingress_port");
	add_required_field("standard_metadata", "packet_length");
	add_required_field("standard_metadata", "instance_type");
	add_required_field("standard_metadata", "egress_spec");
	add_required_field("standard_metadata", "clone_spec");
	add_required_field("standard_metadata", "egress_port");

	force_arith_field("standard_metadata", "ingress_port");
	force_arith_field("standard_metadata", "packet_length");
	force_arith_field("standard_metadata", "instance_type");
	force_arith_field("standard_metadata", "egress_spec");
	force_arith_field("standard_metadata", "clone_spec");

	force_arith_field("queueing_metadata", "enq_timestamp");
	force_arith_field("queueing_metadata", "enq_qdepth");
	force_arith_field("queueing_metadata", "deq_timedelta");
	force_arith_field("queueing_metadata", "deq_qdepth");

	force_arith_field("intrinsic_metadata", "ingress_global_timestamp");
	force_arith_field("intrinsic_metadata", "lf_field_list");
	force_arith_field("intrinsic_metadata", "mcast_grp");
	force_arith_field("intrinsic_metadata", "resubmit_flag");
	force_arith_field("intrinsic_metadata", "egress_rid");
	force_arith_field("intrinsic_metadata", "recirculate_flag");

	import_primitives();
}

int P4Model::init(int argc, char *argv[]) {

	//NS_LOG_FUNCTION(this);
	using ::sswitch_runtime::SimpleSwitchIf;
	using ::sswitch_runtime::SimpleSwitchProcessor;

	// use local call to populate flowtable
	if (P4GlobalVar::g_populateFlowTableWay == LOCAL_CALL)
	{
		this->InitFromCommandLineOptionsLocal(argc, argv, m_argParser);
	}
	else
	{
		// start thrift server , use runtime_CLI populate flowtable
		if (P4GlobalVar::g_populateFlowTableWay == RUNTIME_CLI)
		{
			this->init_from_command_line_options(argc, argv, m_argParser);
			int thriftPort = this->get_runtime_port();
			bm_runtime::start_server(this, thriftPort);
			//NS_LOG_LOGIC("Wait " << P4GlobalVar::g_runtimeCliTime << " seconds for RuntimeCLI operations ");
			std::this_thread::sleep_for(std::chrono::seconds(P4GlobalVar::g_runtimeCliTime));
			//bm_runtime::add_service<SimpleSwitchIf, SimpleSwitchProcessor>("simple_switch", sswitch_runtime::get_handler(this));
		}
	}
	return 0;
}

int P4Model::InitFromCommandLineOptionsLocal(int argc, char *argv[], bm::TargetParserBasic *tp)
{


	//NS_LOG_FUNCTION(this);
	bm::OptionsParser parser;
	parser.parse(argc, argv, tp);
	//NS_LOG_LOGIC("parse pass");
	std::shared_ptr<bm::TransportIface> transport = nullptr;
	int status = 0;
	if (transport == nullptr)
	{
#ifdef BMNANOMSG_ON
		//notifications_addr = parser.notifications_addr;
		transport = std::shared_ptr<bm::TransportIface>(
			TransportIface::make_nanomsg(parser.notifications_addr));
#else
		//notifications_addr = "";
		transport = std::shared_ptr<bm::TransportIface>(bm::TransportIface::make_dummy());
#endif
	}
	if (parser.no_p4)
		status = init_objects_empty(parser.device_id, transport);
	else
		// load p4 json to switch
		status = init_objects(parser.config_file_path, parser.device_id, transport);
	return status;
}

int P4Model::ReceivePacket(Ptr<ns3::Packet> packetIn, int inPort, 
	uint16_t protocol, Address const &destination) 
{
	
	// **************Change ns3::Packet to bm::Packet***************************
	int ns3Length = packetIn->GetSize();
	uint8_t* ns3Buffer = new uint8_t[ns3Length];
	packetIn->CopyData(ns3Buffer,ns3Length);
	std::unique_ptr<bm::Packet> packet = new_packet_ptr(inPort, m_pktID++,
		ns3Length, bm::PacketBuffer(2048, (char*)ns3Buffer, ns3Length));
	delete ns3Buffer;
	// *************************************************************************
	if (packet) {

		int len = packet.get()->get_data_size();
		packet.get()->set_ingress_port(inPort);
		bm::PHV *phv = packet.get()->get_phv();
		phv->reset_metadata();
		phv->get_field("standard_metadata.ingress_port").set(inPort);
		phv->get_field("standard_metadata.packet_length").set(len);

		if (phv->has_field("intrinsic_metadata.ingress_global_timestamp")) {
			phv->get_field("intrinsic_metadata.ingress_global_timestamp").set(0);
		}

		// Ingress
		bm::Parser *parser = this->get_parser("parser");
		bm::Pipeline *ingressMau = this->get_pipeline("ingress");
		phv = packet.get()->get_phv();

		parser->parse(packet.get());

		ingressMau->apply(packet.get());

		packet->reset_exit();

		bm::Field &fEgressSpec = phv->get_field("standard_metadata.egress_spec");
		int egressPort = fEgressSpec.get_int();

		// Egress
		bm::Deparser *deparser = this->get_deparser("deparser");
		bm::Pipeline *egressMau = this->get_pipeline("egress");
		fEgressSpec = phv->get_field("standard_metadata.egress_spec");
		fEgressSpec.set(0);
		egressMau->apply(packet.get());
		deparser->deparse(packet.get());

		// *************************Change bm::Packet to ns3::Packet***********************
		void *bm2Buffer = packet.get()->data();
		size_t bm2Length = packet.get()->get_data_size();
		ns3::Packet ns3Packet((uint8_t*)bm2Buffer,bm2Length);
		Ptr<ns3::Packet> packetOut(&ns3Packet);
		// ********************************************************************************

		m_pNetDevice->SendNs3Packet(packetOut, egressPort, protocol, destination);

		return 0;
	}
	return -1;
}


